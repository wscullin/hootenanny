/*
 * This file is part of Hootenanny.
 *
 * Hootenanny is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --------------------------------------------------------------------
 *
 * The following copyright notices are generated automatically. If you
 * have a new notice to add, please use the format:
 * " * @copyright Copyright ..."
 * This will properly maintain the copyright information. DigitalGlobe
 * copyrights will be updated automatically.
 *
 * @copyright Copyright (C) 2015, 2016 DigitalGlobe (http://www.digitalglobe.com/)
 */
package hoot.services.controllers.osm;

import java.sql.Connection;
import java.sql.SQLException;

import javax.ws.rs.Consumes;
import javax.ws.rs.OPTIONS;
import javax.ws.rs.POST;
import javax.ws.rs.PUT;
import javax.ws.rs.Path;
import javax.ws.rs.PathParam;
import javax.ws.rs.Produces;
import javax.ws.rs.QueryParam;
import javax.ws.rs.WebApplicationException;
import javax.ws.rs.core.MediaType;
import javax.ws.rs.core.Response;
import javax.ws.rs.core.Response.Status;
import javax.xml.transform.dom.DOMSource;

import org.apache.commons.lang3.StringUtils;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.context.support.ClassPathXmlApplicationContext;
import org.springframework.transaction.PlatformTransactionManager;
import org.springframework.transaction.TransactionDefinition;
import org.springframework.transaction.TransactionStatus;
import org.springframework.transaction.support.DefaultTransactionDefinition;
import org.w3c.dom.Document;

import com.mysema.query.sql.SQLQuery;

import hoot.services.db.DbUtils;
import hoot.services.db2.QMaps;
import hoot.services.models.osm.Changeset;
import hoot.services.models.osm.ModelDaoUtils;
import hoot.services.utils.XmlDocumentBuilder;
import hoot.services.validators.osm.ChangesetUploadXmlValidator;
import hoot.services.writers.osm.ChangesetDbWriter;


/**
 * Service endpoint for an OSM changeset
 */
@Path("/api/0.6/changeset")
public class ChangesetResource {
    private static final Logger logger = LoggerFactory.getLogger(ChangesetResource.class);

    private final QMaps maps = QMaps.maps;

    private final PlatformTransactionManager transactionManager;

    public ChangesetResource() {
        ClassPathXmlApplicationContext appContext = new ClassPathXmlApplicationContext(new String[]{"db/spring-database.xml"});
        transactionManager = appContext.getBean("transactionManager", PlatformTransactionManager.class);
    }

    /**
     * Service method endpoint for creating a pre-flight request for a new OSM
     * changeset; required for CORS
     * (http://en.wikipedia.org/wiki/Cross-origin_resource_sharing) support
     * 
     * @return Empty response with CORS headers
     */
    @OPTIONS
    @Path("/create")
    @Consumes(MediaType.APPLICATION_FORM_URLENCODED)
    @Produces(MediaType.TEXT_PLAIN)
    public Response createPreflight() {
        logger.debug("Handling changeset create pre-flight request...");
        return Response.ok().build();
    }

    /**
     * Service method endpoint for creating a new OSM changeset
     * 
     * The Hootenanny Changeset Service implements a subset of the OSM Changeset
     * Service v0.6. It supports the OSM changeset upload process only. It does
     * not support the browsing of changeset contents.
     * 
     * PUT hoot-services/osm/api/0.6/changeset/create?mapId=dc-admin
     * 
     * payload = OSM changeset XML
     * 
     * @param changesetData
     *            changeset create data
     * @param mapId
     *            ID of the map the changeset belongs to
     * @return Response containing the ID assigned to the new changeset
     * @throws Exception
     */
    @PUT
    @Path("/create")
    @Consumes(MediaType.TEXT_XML)
    @Produces(MediaType.TEXT_PLAIN)
    public Response create(String changesetData, @QueryParam("mapId") String mapId) throws Exception {
        Document changesetDoc;
        try {
            logger.debug("Parsing changeset XML...");
            changesetDoc = XmlDocumentBuilder.parse(changesetData);
        }
        catch (Exception ex) {
            String msg = "Error parsing changeset XML: "
                    + StringUtils.abbreviate(changesetData, 100) + " (" + ex.getMessage() + ")";
            throw new WebApplicationException(ex, Response.status(Status.BAD_REQUEST).entity(msg).build());
        }

        Connection conn = DbUtils.createConnection();
        long changesetId = -1;
        try {
            logger.debug("Initializing database connection...");

            long mapIdNum;
            try {
                // input mapId may be a map ID or a map name
                mapIdNum = ModelDaoUtils.getRecordIdForInputString(mapId, conn, maps, maps.id, maps.displayName);
            }
            catch (Exception ex) {
                if (ex.getMessage().startsWith("Multiple records exist")
                        || ex.getMessage().startsWith("No record exists")) {
                    String msg = ex.getMessage().replaceAll("records", "maps").replaceAll("record", "map");
                    throw new WebApplicationException(ex, Response.status(Status.NOT_FOUND).entity(msg).build());
                }

                String msg = "Error requesting map with ID: " + mapId + " (" + ex.getMessage() + ")";
                throw new WebApplicationException(ex, Response.status(Status.BAD_REQUEST).entity(msg).build());
            }

            long userId;
            try {
                logger.debug("Retrieving user ID associated with map having ID: {} ...", mapIdNum);

                SQLQuery query = new SQLQuery(conn, DbUtils.getConfiguration());
                userId = query.from(maps).where(maps.id.eq(mapIdNum)).singleResult(maps.userId);

                logger.debug("Retrieved user ID: {}", userId);
            }
            catch (Exception ex) {
                String msg = "Error locating user associated with map for changeset data: "
                        + StringUtils.abbreviate(changesetData, 100) + " (" + ex.getMessage() + ")";
                throw new WebApplicationException(ex, Response.status(Status.BAD_REQUEST).entity(msg).build());
            }

            logger.debug("Intializing transaction...");
            TransactionStatus transactionStatus = transactionManager
                    .getTransaction(new DefaultTransactionDefinition(TransactionDefinition.PROPAGATION_REQUIRED));
            conn.setAutoCommit(false);

            try {
                changesetId = Changeset.createChangeset(changesetDoc, mapIdNum, userId, conn);
            }
            catch (Exception ex) {
                logger.error("Rolling back the database transaction...");
                transactionManager.rollback(transactionStatus);
                conn.rollback();

                String msg = "Error creating changeset: (" + ex.getMessage() + ") "
                        + StringUtils.abbreviate(changesetData, 100);
                throw new WebApplicationException(ex, Response.status(Status.BAD_REQUEST).entity(msg).build());
            }

            logger.debug("Committing the database transaction...");
            transactionManager.commit(transactionStatus);
            conn.commit();
        }
        finally {
            conn.setAutoCommit(true);
            DbUtils.closeConnection(conn);
        }

        logger.debug("Returning ID: {} for new changeset...", changesetId);

        return Response.ok(String.valueOf(changesetId), MediaType.TEXT_PLAIN)
                .header("Content-type", MediaType.TEXT_PLAIN).build();
    }

    /**
     * Service method endpoint for creating a pre-flight request for uploading
     * changeset diff data;
     * 
     * required for CORS
     * (http://en.wikipedia.org/wiki/Cross-origin_resource_sharing) support
     * 
     * @return Empty response with CORS headers
     */
    @OPTIONS
    @Path("/{changesetId}/upload")
    @Consumes(MediaType.APPLICATION_FORM_URLENCODED)
    @Produces(MediaType.TEXT_PLAIN)
    public Response uploadPreflight() {
        logger.debug("Handling changeset upload pre-flight request...");
        return Response.ok().build();
    }

    /**
     * Service method endpoint for uploading OSM changeset diff data
     * 
     * @param changeset
     *            OSM changeset diff data
     * @param changesetId
     *            ID of the changeset being uploaded; changeset with the ID must
     *            already exist
     * @param mapId
     *            ID of the map owning the changeset being uploaded
     * @return response acknowledging the result of the update operation with
     *         updated entity ID information
     * @throws Exception
     */
    @POST
    @Path("/{changesetId}/upload")
    @Consumes(MediaType.TEXT_XML)
    @Produces(MediaType.TEXT_XML)
    public Response upload(String changeset,
                           @PathParam("changesetId") long changesetId,
                           @QueryParam("mapId") String mapId) throws Exception {
        logger.debug("Intializing database connection...");
        Connection conn = DbUtils.createConnection();
        Document changesetUploadResponse = null;
        try {
            logger.debug("Intializing changeset upload transaction...");
            TransactionStatus transactionStatus = transactionManager
                    .getTransaction(new DefaultTransactionDefinition(TransactionDefinition.PROPAGATION_REQUIRED));
            conn.setAutoCommit(false);

            try {
                if (mapId == null) {
                    throw new Exception("Invalid map id.");
                }

                long mapid = Long.parseLong(mapId);
                Document changesetDoc;
                try {
                    changesetDoc = (new ChangesetUploadXmlValidator()).parseAndValidate(changeset);
                }
                catch (Exception e) {
                    throw new Exception("Error parsing changeset diff data: " + StringUtils.abbreviate(changeset, 100)
                            + " (" + e.getMessage() + ")", e);
                }
                changesetUploadResponse = (new ChangesetDbWriter(conn)).write(mapid, changesetId, changesetDoc);
            }
            catch (Exception e) {
                logger.error("Rolling back transaction for changeset upload...", e);
                transactionManager.rollback(transactionStatus);
                conn.rollback();
                handleError(e, changesetId, StringUtils.abbreviate(changeset, 100));
            }

            logger.debug("Committing changeset upload transaction...");
            transactionManager.commit(transactionStatus);
            conn.commit();
        }
        finally {
            conn.setAutoCommit(true);
            DbUtils.closeConnection(conn);
        }

        logger.debug("Returning changeset upload response: {} ...",
                StringUtils.abbreviate(XmlDocumentBuilder.toString(changesetUploadResponse), 100));

        return Response.ok(new DOMSource(changesetUploadResponse), MediaType.TEXT_XML)
                       .header("Content-type", MediaType.TEXT_XML).build();
    }

    /**
     * Service method endpoint for creating a pre-flight request for the closing
     * a changeset; required for CORS
     * (http://en.wikipedia.org/wiki/Cross-origin_resource_sharing) support
     * 
     * @return Empty response with CORS headers
     */
    @OPTIONS
    @Path("/{changesetId}/close")
    @Consumes(MediaType.APPLICATION_FORM_URLENCODED)
    public Response closePreflight() {
        logger.info("Handling changeset close pre-flight request...");

        return Response.ok().build();
    }

    /**
     * Service method endpoint for closing a changeset
     * 
     * @param changesetId
     *            ID of the changeset to close
     * @return HTTP status code indicating the status of the closing of the
     *         changeset
     * @throws Exception
     */
    @PUT
    @Path("/{changesetId}/close")
    @Consumes(MediaType.TEXT_PLAIN)
    @Produces(MediaType.TEXT_PLAIN)
    public String close(@PathParam("changesetId") long changesetId,
                        @QueryParam("mapId") String mapId) throws Exception {
        logger.info("Closing changeset with ID: {} ...", changesetId);

        Connection conn = DbUtils.createConnection();
        try {
            logger.debug("Intializing database connection...");
            if (mapId == null) {
                throw new Exception("Invalid map id.");
            }
            long mapid = Long.parseLong(mapId);

            Changeset.closeChangeset(mapid, changesetId, conn);
        }
        finally {
            DbUtils.closeConnection(conn);
        }

        return Response.status(Status.OK).toString();
    }

    // TODO: clean up these message...some are obsolete now
    public static void handleError(Exception e, long changesetId, String changesetDiffSnippet) {
        String message = e.getMessage();
        if (e instanceof SQLException) {
            SQLException sqlException = (SQLException) e;
            if (sqlException.getNextException() != null) {
                message += "  " + sqlException.getNextException().getMessage();
            }
        }
        if (e.getCause() instanceof SQLException) {
            SQLException sqlException = (SQLException) e.getCause();
            if (sqlException.getNextException() != null) {
                message += "  " + sqlException.getNextException().getMessage();
            }
        }

        // To make the error checking code cleaner and simpler, if an element is
        // referenced in an update or delete changeset that doesn't exist in the database, we're
        // not differentiating between whether it was an element, relation member, or way node
        // reference. Previously, we'd throw a 412 if the non-existing element was a relation member or way
        // node reference and a 404 otherwise. Now, we're always throwing a 404. This shouldn't be a big
        // deal, b/c the hoot UI doesn't differentiate between the two types of failures.
        if (!StringUtils.isEmpty(e.getMessage())) {
            if (e.getMessage().contains("Invalid changeset ID") || e.getMessage().contains("Invalid version")
                    || e.getMessage().contains("references itself")
                    || e.getMessage().contains("Changeset maximum element threshold exceeded")
                    || e.getMessage().contains("was closed at")) {
                throw new WebApplicationException(Response.status(Status.CONFLICT).entity(message).build());
            }
            else if (e.getMessage().contains("to be updated does not exist")
                    || e.getMessage().contains("Element(s) being referenced don't exist.")) {
                throw new WebApplicationException(Response.status(Status.NOT_FOUND).entity(message).build());
            }
            else if (e.getMessage().contains("exist specified for") || e.getMessage().contains("exist for")
                    || e.getMessage().contains("still used by") || e.getMessage()
                            .contains("One or more features in the changeset are involved in an unresolved review")) {
                throw new WebApplicationException(Response.status(Status.PRECONDITION_FAILED).entity(message).build());
            }
        }

        String msg = "Error uploading changeset with ID: " + changesetId + " - data: (" + message
                + ") " + changesetDiffSnippet;
        throw new WebApplicationException(Response.status(Status.BAD_REQUEST).entity(msg).build());
    }
}
